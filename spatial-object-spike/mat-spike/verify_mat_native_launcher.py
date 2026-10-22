"""Independent own-child cleanup/output oracle; never runs native/audiodg mode."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_cpp_mat_stream import digest, load, require
from verify_cpp_mat_ipc import wait_own_receiver

HEADER = struct.Struct('<8s4IQ')
FRAME = struct.Struct('<4I2q')
CALL = struct.Struct('<4I3fI480f')
RECORD = struct.Struct('<IIQqQIIIIiIQ')


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf8')


def source_offsets(raw):
    require(HEADER.unpack_from(raw)[:5] == (b'MATBRG1\0', 1, 48000, 480, HEADER.unpack_from(raw)[4]), 'source header')
    total = HEADER.unpack_from(raw)[4]
    offset, frames, calls, samples = HEADER.size, [], 0, 0
    for n in range(total):
        frame = FRAME.unpack_from(raw, offset)
        require(frame[0] == n and frame[1] == 480 and frame[3] == int(n+1 == total), 'source frame oracle')
        offset += FRAME.size
        entries = []
        for _ in range(frame[2]):
            c = CALL.unpack_from(raw, offset)
            require(c[0] > 0 and c[2] == 1 and c[3] in (6, 7, 8) and c[7] == 480, 'source object oracle')
            entries.append(offset)
            if n+1 < total and c[3] != 8:
                samples += 480
            calls += 1
            offset += CALL.size
        frames.append((frame, entries))
    require(offset == len(raw), 'source exact byte length')
    return frames, calls, samples


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'bridge', 'preflight', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    ap.add_argument('--repeats', type=int, default=10)
    args = ap.parse_args()
    require(1 <= args.repeats <= 20, 'bounded selftest repetitions')
    args.output.mkdir(parents=True, exist_ok=False)
    build = load(args.build/'build.json')
    exe = (args.build/'mat-native-launcher.exe').resolve()
    dll = Path(build['receiver_dll'])
    require(digest(exe).upper() == build['executable_sha256']
            and digest(dll).upper() == build['receiver_dll_sha256'], 'frozen binaries changed')
    for manifest, root in ((build, args.build.parent), (load(dll.parent/'build.json'), dll.parent.parent)):
        for item in manifest['source_files']:
            require(digest(root/item['file']).upper() == item['sha256'], 'frozen source changed')
    preflight = load(args.preflight/'preflight.json')
    inspection = load(args.preflight/'inspection/inspection.json')
    require(preflight['preflight_completed'] and preflight['mode'] == 'Preflight'
            and preflight['executable_sha256'] == build['executable_sha256']
            and not preflight['audiodg_opened'] and not preflight['staging_performed']
            and not preflight['native_encoder_called'] and not inspection['foreign_process_opened']
            and not inspection['module_loaded'] and not inspection['engine_started'], 'preflight scope')
    script = args.build.parent/'run-mat-native-session-once.ps1'
    require(digest(script).upper() == preflight['launcher_script_sha256'], 'preflight script changed')

    def invoke(arguments, expected):
        proc = subprocess.Popen([str(exe), *map(str, arguments)], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            stdout, stderr = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired as e:
            raise RuntimeError(f'Own launcher {proc.pid} exceeded bound; not killed/retried') from e
        require(proc.returncode == expected, f'own launcher {arguments[0]} exit {proc.returncode}: '+stderr.decode(errors='replace'))
        return dict(pid=proc.pid, exit_code=proc.returncode, stdout=stdout.decode(errors='replace'), stderr=stderr.decode(errors='replace'))

    source = (args.bridge/'domain-1-mat-object-input.bin').resolve()
    repeats = []
    for n in range(args.repeats):
        directory = args.output/f'repeat-{n+1}'
        execution = invoke(['--selftest', dll, source, directory.resolve()], 0)
        proof = load(directory/'proof.json')
        require(proof['passed'] and proof['selftests_passed'] == 15 and not proof['audiodg_accessed']
                and not proof['foreign_process_accessed'] and not proof['native_encoder_called']
                and not proof['vendor_loaded'] and proof['output_fixture_is_synthetic'], 'own-child test scope')
        expected = RECORD.pack(0x42595445, 0, 0, 10, 1234, 480, 30720, 0, 123, 0, 0, 0)
        expected += RECORD.pack(0x42595445, 5, 1, 20, 1234, 480, 30720, 5, 123, 0, 0, 0)+b'abcde'
        require((directory/'encoder-output.bin').read_bytes() == expected
                and (directory/'encoder-payload.raw').read_bytes() == b'abcde', 'independent record/payload bytes')
        children = []
        for i in range(6):
            d = directory/f'handoff-{i}'
            r, child = load(d/'summary.json'), load(d/'child-exit.json')
            c = r['cleanup']
            require(child['exit_code'] == 0 and not child['forced_termination']
                    and child['handles_before_entry'] == child['handles_after_entry']
                    and c['entry_finished'] and c['loader_finished'] and c['close_error'] == 0
                    and not c['argument_retained'] and c['retained_handle_count'] == 0
                    and c['remote_original_handles'] == c['remote_original_handles_released'] == (5 if i == 0 else i-1),
                    'remote original handle/argument cleanup')
            if i == 0:
                h = r['output']
                require(r['entry_return'] == h['error'] == 10 and h['status'] == -1
                        and not h['host_guard_passed'] and not h['helper_pinned']
                        and h['vendor_loads'] == h['activations'] == h['initializations'] == h['worker_tid'] == 0,
                        'ordinary child must reject before vendor/worker')
            exited = wait_own_receiver(dict(consumer_pid=child['pid'], consumer_creation_time=child['creation_time']))
            children.append(dict(pid=child['pid'], cleanup=exited,
                                 handles_before_DLL=child['handles_before_DLL_load'],
                                 handles_before_entry=child['handles_before_entry'], handles_after_entry=child['handles_after_entry']))
        repeats.append(dict(launcher_pid=execution['pid'], checks=15, children=children, output_bytes_exact=True))

    source_checks = []
    for domain, expected_rounds, expected_samples in ((1, 306, 288000), (2, 206, 96000)):
        path = args.bridge/f'domain-{domain}-mat-object-input.bin'
        raw = path.read_bytes()
        frames, calls, samples = source_offsets(raw)
        d = args.output/f'validate-domain-{domain}'
        invoke(['--validate-source', path.resolve(), d.resolve()], 0)
        report = load(d/'source.json')
        require(report['source_rounds'] == len(frames)-1 == expected_rounds
                and report['source_samples'] == samples == expected_samples and report['calls'] == calls
                and report['source_sha256'] == digest(path), 'independent bounded source parser counts')
        source_checks.append(report)
    raw = source.read_bytes()
    frames, _, _ = source_offsets(raw)
    first = next(entries for _, entries in frames if entries)
    require(len(first) >= 2, 'two-object corruption fixture')

    def mutate(offset, fmt, value):
        changed = bytearray(raw)
        struct.pack_into(fmt, changed, offset, value)
        return bytes(changed)

    corruptions = dict(truncated_header=raw[:15], truncated_PCM=raw[:-1], trailing_byte=raw+b'x',
                       bad_version=mutate(8, '<I', 2), oversized_round_count=mutate(20, '<I', 352),
                       unknown_initial_update=mutate(first[0]+12, '<I', 6),
                       duplicate_object=mutate(first[1], '<I', CALL.unpack_from(raw, first[0])[0]),
                       nonfinite_XYZ=mutate(first[0]+16, '<I', 0x7fc00000),
                       nonfinite_PCM=mutate(first[0]+32, '<I', 0x7fc00000),
                       oversized_PCM=mutate(first[0]+32, '<f', 1.5),
                       missing_EOF_flag=mutate(len(raw)-FRAME.size-len(frames[-1][1])*CALL.size+12, '<I', 0))
    failures = []
    for name, content in corruptions.items():
        path = args.output/(name+'.bin')
        path.write_bytes(content)
        d = args.output/(name+'-rejected')
        execution = invoke(['--validate-source', path.resolve(), d.resolve()], 3)
        require(not (d/'source.json').exists(), 'invalid source reported success')
        failures.append(dict(name=name, rejected_before_process_access=True, diagnostic=execution['stderr']))
    result = dict(passed=True, executable_sha256=build['executable_sha256'], oracle_sha256=digest(Path(__file__)),
                  repetitions=repeats, selftest_checks=args.repeats*15, own_children=args.repeats*6,
                  source_checks=source_checks, rejected_sources=failures,
                  preflight_ready_for_authorized_attempt=preflight['ready_for_authorized_attempt'],
                  preflight_blockers=preflight['blockers'], audiodg_accessed=False, native_encoder_called=False,
                  native_entry_guard_tested_in_own_children=True, vendor_loaded=False, mat_verified=False,
                  forced_termination=False, native_branch_not_executed=True,
                  caveat='Native mode compiled/prepared only. Own-child guard, partial handoff and synthetic output cannot prove real engine permissions/encoding.')
    save(args.output/'proof.json', result)
    print(json.dumps(dict(passed=True, selftest_checks=args.repeats*15, own_children=args.repeats*6,
                          rejected_sources=len(failures), native_encoder_called=False), indent=2))


if __name__ == '__main__':
    main()
