"""M1/M2/M3 independent oracle; own fixtures only, never native/audiodg mode."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from verify_cpp_mat_stream import B, O, digest, frozen_plans, load, require
from verify_cpp_mat_ipc import wait_own_receiver

ROOT = Path(__file__).resolve().parent
FRAME = struct.Struct('<4I2q')


def save(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf8')


def normalized_frames(raw):
    out, offset = bytearray(), 0
    while offset < len(raw):
        index, frames, calls, flags, first, last = FRAME.unpack_from(raw, offset)
        size = FRAME.size+calls*1952
        require(frames == 480 and calls <= 64 and flags in (0, 1) and offset+size <= len(raw), 'bridge frame oracle')
        out.extend(FRAME.pack(index, frames, calls, flags, 0, 0))
        out.extend(raw[offset+FRAME.size:offset+size])
        offset += size
    require(offset == len(raw), 'whole bridge frame oracle')
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    build = load(args.build/'build.json')
    for item in build['source_files']:
        require(digest(ROOT/item['file']).upper() == item['sha256'], 'compiled source changed: '+item['file'])
    for item in build['binaries']+build['runtime']:
        require(digest(args.build/item['file']).upper() == item['sha256'], 'compiled binary changed')
    exe = (args.build/'spatial-audio-broker.exe').resolve()
    fixture = (args.build/'spatial-capture-fixture.exe').resolve()
    adapter = (args.build/'spatial_capture_adapter.dll').resolve()
    dll = (args.build/'mat_stream_receiver.dll').resolve()
    capture = ROOT.parent/'resident/quantum-v4-ordered-native01/capture'
    source_proof = load(ROOT.parent/'resident/quantum-v4-ordered-content01/proof.json')
    require(source_proof['complete_encoder_input_capture_contract_passed'], 'native source proof')
    for item in source_proof['source_files']:
        require(digest(capture/item['file']) == item['sha256'], 'frozen capture changed')
    fixture_raw = (ROOT/'mat-native-v4-input01/input.bin').read_bytes()
    fixture_proof = load(ROOT/'mat-native-v4-input-proof01/proof.json')
    require(digest(ROOT/'mat-native-v4-input01/input.bin') == fixture_proof['input_sha256'], 'native-tested call oracle')
    plans = frozen_plans(fixture_raw)
    golden = ROOT/'cpp-native-v4-replay02'
    golden_summary = load(golden/'summary.json')
    for d in golden_summary['domains']:
        require(digest(golden/d['file']) == d['sha256'], 'golden bridge changed')

    def run(command, expected=0, timeout=40):
        p = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            stdout, stderr = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as e:
            raise RuntimeError(f'Own fixture {p.pid} exceeded bound; not killed/retried') from e
        require(p.returncode == expected, f'own fixture exit {p.returncode}, expected {expected}: '+stderr.decode(errors='replace'))
        return dict(pid=p.pid, exit=p.returncode, stdout=stdout.decode(errors='replace'), stderr=stderr.decode(errors='replace'))

    selftest_dir = args.output/'selftest'
    run([exe, '--selftest', selftest_dir.resolve()])
    require(load(selftest_dir/'proof.json')['checks'] == 10, 'ring/source selftests')
    replay_input = ROOT/'mat-native-session-run02/capture/encoder-payload.raw'
    require(digest(replay_input) == '79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6', 'frozen real MAT replay bytes')
    replay_dir = args.output/'replay'
    run([exe, '--replay', replay_input, replay_dir.resolve()])
    require((replay_dir/'replayed.raw').read_bytes() == replay_input.read_bytes(), 'M1 replay lost/changed bytes')
    negative_replays = []
    for name, data in [('partial', replay_input.read_bytes()[:-1]), ('preamble', bytes(4)+replay_input.read_bytes()[4:]), ('empty', b'')]:
        path = args.output/(name+'.raw')
        path.write_bytes(data)
        run([exe, '--replay', path.resolve(), (args.output/(name+'-rejected')).resolve()], 2)
        negative_replays.append(name)

    runs = []
    for name, repeat, paced in [('normal', 1, 0), ('paced', 1, 1), ('wrap', 5, 0), ('idle', 1, 0)]:
        directory = args.output/name
        run([fixture, exe, adapter, dll, capture, directory.resolve(), repeat, paced, name if name == 'idle' else 'normal'])
        launch, summary, broker = load(directory/'launch.json'), load(directory/'proof.json'), load(directory/'broker/proof.json')
        require(summary['passed'] and broker['passed'] and launch['inherited_handles'] == 3
                and summary['events_published'] == summary['events_consumed'] == 3140*repeat
                and broker['capture_events'] == 3140*repeat and broker['source_rounds'] == 512*repeat
                and broker['closed_domains'] == 2*repeat
                and launch['capture_args_bytes'] == broker['capture_args_bytes']
                and launch['capture_event_bytes'] == broker['capture_event_bytes'], 'actual MSVC/GCC C ABI pipeline')
        domains = []
        for r in range(repeat):
            for meta, expected, counts in plans:
                original_domain = meta[0]
                domain = original_domain+2*r
                data = (directory/f'broker/domain-{domain}.plan').read_bytes()
                require(data == expected, 'actual streaming fake Submit/Process differs from native-tested PCM/XYZ/lifecycle')
                frames = (directory/f'broker/domain-{domain}.frames').read_bytes()
                original_frames = (golden/f'domain-{original_domain}-mat-object-input.bin').read_bytes()[32:]
                require(normalized_frames(frames) == normalized_frames(original_frames), 'memory bridge differs from golden')
                if r == 0:
                    require(frames == original_frames, 'first-pass timestamps/whole bridge bytes changed')
                stat = load(directory/f'broker/domain-{domain}.json')
                require(stat['completed'] and stat['source_rounds'] == meta[2]
                        and stat['output_blocks'] == stat['consumed_blocks'] == meta[1]
                        and stat['submissions'] == counts['submissions']
                        and stat['source_samples'] == counts['source_audio_samples']
                        and stat['worker_finished'] and not stat['worker_timed_out'], 'streaming receiver counters')
                domains.append(dict(domain=domain, transcript_exact=True, bridge_exact=True, output_ring_wraps=stat['ring_wraps']))
        cleanup = wait_own_receiver(launch)
        runs.append(dict(mode=name, repeated_captures=repeat, domains=domains, input_ring_wraps=summary['input_ring_wraps'], cleanup=cleanup))
    failures = []
    for mode in ('header-version', 'bad-owner', 'loss', 'sequence', 'missing-close'):
        directory = args.output/mode
        run([fixture, exe, adapter, dll, capture, directory.resolve(), 1, 0, mode], 2)
        summary, launch = load(directory/'proof.json'), load(directory/'launch.json')
        require(not summary['passed'] and summary['consumer_exited'] and summary['consumer_exit'] == 2
                and not (directory/'broker/proof.json').exists(), 'bad capture was accepted')
        failures.append(dict(mode=mode, rejected=True, cleanup=wait_own_receiver(launch)))
    owner_dir = args.output/'owner-exit'
    run([fixture, exe, adapter, dll, capture, owner_dir.resolve(), 1, 0, 'owner-exit'], 23)
    cleanup = wait_own_receiver(load(owner_dir/'launch.json'))
    require(load(owner_dir/'intentional-exit.json')['intentional_own_fixture_exit']
            and (owner_dir/'broker/failure.json').exists() and not (owner_dir/'broker/proof.json').exists(), 'owner exit must fault capture source')
    failures.append(dict(mode='owner-exit', rejected=True, cleanup=cleanup))
    filters = []
    for mode in ('selected-none', 'self-excluded'):
        directory = args.output/mode
        run([fixture, exe, adapter, dll, capture, directory.resolve(), 1, 0, mode])
        proof = load(directory/'broker/proof.json')
        require(proof['passed'] and proof['excluded_domains'] > 0 and proof['closed_domains'] < 2, 'source domain filter ignored')
        if mode == 'selected-none':
            require(proof['source_rounds'] == proof['closed_domains'] == 0, 'unselected domain reached encoder')
        filters.append(dict(mode=mode, excluded_domains=proof['excluded_domains'], native_hook_coexistence_verified=False))
        wait_own_receiver(load(directory/'launch.json'))
    stream_dir = args.output/'stream-tests'
    run([exe, '--stream-tests', dll, stream_dir.resolve()])
    stream = load(stream_dir/'proof.json')
    require(stream['passed'] and stream['long_case_source_rounds'] == 1200 and len(stream['cases']) == 8, 'M3 continuous/failure tests')
    # Independently reconstruct the >400-round synthetic fake transcript.
    expected = bytearray(B.pack(2, 0, 0))
    for n in range(20):
        expected.extend(B.pack(1, 0xffffffff, 1))
        expected.extend(O.pack(1, 7 if n == 0 else 6, 0.01, 0, 0, *([0.]*480)))
    for n in range(1, 1200):
        pcm = [((n+j) % 100)/1000 for j in range(480)]
        expected.extend(B.pack(2, n, 1))
        expected.extend(O.pack(1, 6, (n % 100)/100, 0, 0, *pcm))
    for n in range(30):
        expected.extend(B.pack(3, 0xffffffff, int(n == 0)))
        if n == 0:
            expected.extend(O.pack(1, 8, 0, 0, 0, *([0.]*480)))
    require((stream_dir/'case-0/domain-1.plan').read_bytes() == expected, 'continuous transcript PCM/XYZ/lifecycle not exact')
    result = dict(passed=True, build=str(args.build.resolve()), source_oracle_sha256=digest(ROOT/'mat-native-v4-input01/input.bin'),
                  verifier_sha256=digest(Path(__file__)), M1=dict(replayed_bytes=10936320, frames=178, byte_exact=True, invalid_replays=negative_replays),
                  M2=dict(runs=runs, rejected_captures=failures, filters=filters),
                  M3=dict(output_mapping_bytes=994752, slots=8, ring_unit_iterations=100000,
                          continuous_source_rounds=1200, continuous_Process_blocks=1250, continuous_transcript_exact=True,
                          failure_cases=7, ordinary_host_native_guard_only=True),
                  native_encoder_called=False, audiodg_accessed=False, deployment=False,
                  real_continuous_native_encoding_verified=False, RTP_implemented=False, Android_verified=False,
                  source_PTS_calibrated=False, default_Sunshine_audio_changed=False, forced_termination=False)
    save(args.output/'proof.json', result)
    print(json.dumps(dict(passed=True, M1_replay_bytes=10936320, M2_complete_domain_replays=sum(len(r['domains']) for r in runs),
                          M3_continuous_Process_blocks=1250, native_encoder_called=False), indent=2))


if __name__ == '__main__':
    main()
