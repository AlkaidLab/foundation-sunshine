"""Own-process IPC exercise + independent byte oracle, no audio/vendor calls.

Only launches the supplied frozen test executable. Abrupt-exit fixtures exit
themselves; the harness never kills a process or opens an unrelated PID.
"""
import argparse
import csv
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess

from verify_cpp_mat_stream import B, C, H, O, digest, frozen_plans, load, parse_plan, require, sha


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf8')


def wait_own_receiver(launch):
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    api.OpenProcess.restype = wintypes.HANDLE
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    api.WaitForSingleObject.restype = wintypes.DWORD
    api.GetProcessTimes.argtypes = [wintypes.HANDLE, *([ctypes.POINTER(wintypes.FILETIME)]*4)]
    api.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    h = api.OpenProcess(0x100000 | 0x1000, False, launch['consumer_pid'])
    if not h:
        require(ctypes.get_last_error() == 87, 'cannot audit own receiver exit')
        return dict(exited=True, handle_already_gone=True)
    try:
        times = [wintypes.FILETIME() for _ in range(4)]
        require(api.GetProcessTimes(h, *[ctypes.byref(t) for t in times]), 'own receiver identity read')
        created = times[0].dwLowDateTime | (times[0].dwHighDateTime << 32)
        if created != launch['consumer_creation_time']:
            return dict(exited=True, pid_reused_after_recorded_child_exit=True)
        require(api.WaitForSingleObject(h, 5000) == 0, 'own receiver did not exit; not forcibly terminated')
        code = wintypes.DWORD()
        require(api.GetExitCodeProcess(h, ctypes.byref(code)), 'own child exit code')
        return dict(exited=True, exit_code=code.value, identity_creation_time=created)
    finally:
        api.CloseHandle(h)


def rows(path):
    with path.open(newline='') as stream:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(stream)]


def check_clock(directory, raw, info, launch):
    parent = rows(directory/'producer-clock.csv')
    ingress = rows(directory/'receiver/ingress.csv')
    process = rows(directory/'receiver/process-clock.csv')
    require(len(parent) == len(ingress) == launch['source_records'], 'IPC source count')
    freq = launch['qpc_frequency']
    for n, (a, b) in enumerate(zip(parent, ingress)):
        require(a['index'] == b['index'] == n and b['kind'] == (2 if n+1 == len(parent) else 1)
                and a['enqueued_qpc'] >= a['target_qpc'] and a['send_finish_qpc'] >= a['enqueued_qpc']
                and a['enqueued_qpc'] == b['enqueued_qpc'] and b['received_qpc'] >= a['enqueued_qpc'], 'IPC clock/identity order')
        if n:
            require(abs(a['target_qpc']-parent[n-1]['target_qpc']-freq/100) <= 1, 'producer cadence')
    offset = source = 0
    latency = []
    for row in process:
        phase, index, count = B.unpack_from(raw, offset)
        offset += B.size+count*O.size
        require((row['phase'], row['source_index']) == (phase, index)
                and row['finish_qpc'] >= row['start_qpc'], 'actual fake Process transcript clock')
        if phase == 2:
            require(index == source and row['finish_qpc'] >= ingress[index]['received_qpc'], 'Process after own message receipt')
            latency.append((row['finish_qpc']-parent[index]['enqueued_qpc'])*1000/freq)
            source += 1
    require(offset == len(raw) and source == len(parent)-1 and len(process) == info['scheduled_rounds'], 'EOF became source Process')
    latency.sort()
    return dict(producer_rows=len(parent), source_Process_rows=source, actual_Process_rows=len(process),
                ingress_to_fake_Process_finish_ms=dict(min=latency[0], p50=latency[len(latency)//2], max=latency[-1]),
                wall_clock_only=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'bridge', 'input', 'input-proof', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    build = load(args.build/'build.json')
    exe = (args.build/'mat-ipc-replay.exe').resolve()
    require(digest(exe).upper() == build['executable_sha256'], 'frozen test executable changed')
    for entry in build['source_files']:
        require(digest(args.build.parent/entry['file']).upper() == entry['sha256'], 'build source changed')
    proof = load(args.input_proof/'proof.json')
    raw = (args.input/'input.bin').read_bytes()
    require(proof['passed'] and sha(raw) == proof['input_sha256']
            and digest(args.input/'manifest.json') == proof['input_manifest_sha256'], 'frozen native-tested input')
    plans = frozen_plans(raw)
    bridge = load(args.bridge/'summary.json')
    require(bridge['offline_bridge_replay_passed'] and len(bridge['domains']) == 2, 'bridge golden')
    for d in bridge['domains']:
        require(digest(args.bridge/d['file']) == d['sha256'], 'bridge source changed')
    runs, failures = [], []

    def run(mode, domain, epoch):
        directory = args.output/f'{mode}-domain-{domain}-epoch-{epoch}'
        source = args.bridge/f'domain-{domain}-mat-object-input.bin'
        proc = subprocess.Popen([str(exe), '--produce', str(source.resolve()), str(domain), str(epoch),
                                 str(directory.resolve()), mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            stdout, stderr = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired as e:
            raise RuntimeError(f'Own helper {proc.pid} exceeded bound; not killed or retried') from e
        launch = load(directory/'launch.json')
        require(launch['producer_pid'] == proc.pid and launch['consumer_pid'] != proc.pid
                and launch['explicit_inherited_handle_count'] == 5 and not launch['named_global_objects']
                and not launch['security_settings_changed'], 'own-process/handle scope')
        cleanup = wait_own_receiver(launch)
        save(directory/'harness-exit.json', dict(producer_exit=proc.returncode, receiver=cleanup,
                                               stderr=stderr.decode(errors='replace'), stdout=stdout.decode(errors='replace'),
                                               forced_termination=False))
        return directory, launch, proc.returncode, cleanup

    for mode in ('normal', 'delayed'):
        rebuilt = bytearray(raw[:H.size])
        domains = []
        for meta, expected, counts in plans:
            domain = meta[0]
            directory, launch, code, cleanup = run(mode, domain, 1000+len(runs)*100+domain)
            p = load(directory/'summary.json')
            r = load(directory/'receiver/summary.json')
            actual = (directory/'receiver/fake-plan.bin').read_bytes()
            require(code == 0 and p['completed'] and r['completed'] and p['fault'] == r['fault'] == 0
                    and p['consumer_exited'] and p['consumer_exit'] == 0 and r['guard_passed'] and r['eof_seen']
                    and p['producer_backpressure'] is False and p['producer_capacity_retries'] == p['rejected'] == 0
                    and p['write'] == p['read'] == p['accepted'] == r['messages_consumed'] == meta[2]+1,
                    'two-process completion/EOF')
            require(actual == expected and sha(actual) == r['fake_plan_sha256'] and parse_plan(actual, meta[1]) == counts,
                    'consumer PCM/XYZ/control transcript differs from prior native-tested input')
            source = args.bridge/f'domain-{domain}-mat-object-input.bin'
            require((directory/'receiver/source.bin').read_bytes() == source.read_bytes()
                    and r['source_sha256'] == digest(source), 'source including EOF changed')
            clock = check_clock(directory, actual, r, launch)
            rebuilt.extend(C.pack(*meta))
            rebuilt.extend(actual)
            domains.append(dict(domain=domain, epoch=launch['epoch'], producer_pid=launch['producer_pid'],
                                consumer_pid=launch['consumer_pid'], whole_transcript_byte_identical=True,
                                ring_high_water=p['ring_high_water'], queue_high_water=r['queue_high_water'],
                                cleanup=cleanup, clock=clock, **counts))
        require(bytes(rebuilt) == raw, 'whole prior native input not reproduced')
        runs.append(dict(mode=mode, reconstructed_input_sha256=sha(rebuilt), domains=domains))
    expected_errors = {'header-version': 1, 'contract-change': 1, 'identity': 2, 'frame-epoch': 2, 'frame-length': 3,
                       'checksum': 4, 'sequence': 5, 'overflow': 6, 'cancel': 7, 'owner-exit': 8,
                       'consumer-exit': 9, 'owner-stale': 10, 'consumer-stale': 11, 'missing-eof': 12}
    for n, (mode, expected) in enumerate(expected_errors.items()):
        directory, launch, code, cleanup = run(mode, 1, 2000+n)
        if mode == 'owner-exit':
            require(code == 23 and load(directory/'abrupt-exit.json')['intentional_own_helper_exit'], 'own producer abrupt-exit fixture')
        else:
            p = load(directory/'summary.json')
            require(code == 2 and not p['completed'] and p['fault'] == expected and p['consumer_exited'], f'{mode} producer failure not propagated')
        if mode == 'consumer-exit':
            require(load(directory/'receiver/abrupt-exit.json')['code'] == 31, 'own consumer abrupt-exit fixture')
        else:
            r = load(directory/'receiver/summary.json')
            require(not r['completed'] and r['fault'] == expected and not r['native_encoder_called'], f'{mode} receiver failure not propagated')
            if mode in ('header-version', 'identity'):
                require(not r['guard_passed'] and r['encoder_opens'] == 0, 'invalid contract reached codec')
        failures.append(dict(mode=mode, expected_error=expected, propagated=True, own_processes_exited=cleanup['exited']))
    result = dict(passed=True, protocol_version=1, executable_sha256=build['executable_sha256'],
                  oracle_sha256=digest(Path(__file__)), native_tested_input_sha256=sha(raw), normal_runs=runs,
                  failure_cases=failures, native_encoder_called=False, mat_encoded=False,
                  live_capture_verified=False, audiodg_accessed=False, forced_termination=False,
                  caveat='Two self-owned normal processes only; no cross-token/PPL/real MAT/Android claim. Startup buffer latency remains.')
    save(args.output/'proof.json', result)
    print(json.dumps(dict(passed=True, full_domain_replays=4, failure_cases=len(failures),
                          native_tested_input_sha256=sha(raw), native_encoder_called=False), indent=2))


if __name__ == '__main__':
    main()
