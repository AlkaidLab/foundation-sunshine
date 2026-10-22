"""Read-only verification of the ordinary-process DLL adapter tests/replay.
No process launch, audio activation, proprietary loading or input mutation.
"""
import argparse
import json
from pathlib import Path

from verify_cpp_mat_stream import H, C, digest, frozen_plans, load, parse_plan, require, sha


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'replay', 'input', 'input-proof', 'ipc-proof', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    build = load(args.build/'build.json')
    require(digest(args.build/'mat_ipc_receiver.dll').upper() == build['dll_sha256']
            and digest(args.build/'mat-ipc-receiver-test.exe').upper() == build['executable_sha256'], 'build binary changed')
    for entry in build['source_files']:
        require(digest(args.build.parent/entry['file']).upper() == entry['sha256'], 'build source changed')
    tests = load(args.build/'selftest/proof.json')
    require(tests['passed'] and tests['selftests_passed'] == 23 and not tests['vendor_loaded']
            and not tests['native_encoder_called'] and not tests['process_attached'], 'ordinary test scope')
    cpp_proof = load(args.ipc_proof/'proof.json')
    cpp_layout = load(args.ipc_proof/'normal-domain-1-epoch-1001/launch.json')
    require(cpp_proof['passed'] and cpp_proof['protocol_version'] == 1, 'previous IPC proof')
    for key in ('map_bytes', 'slot_bytes', 'message_bytes'):
        require(tests[key] == cpp_layout[key], 'MSVC standalone vs frozen GCC layout mismatch')
    require(tests['call_bytes'] == 1952, 'wire call ABI')
    proof = load(args.input_proof/'proof.json')
    raw = (args.input/'input.bin').read_bytes()
    require(proof['passed'] and sha(raw) == proof['input_sha256']
            and digest(args.input/'manifest.json') == proof['input_manifest_sha256'], 'frozen native input changed')
    for entry in proof['expected_files']:
        require(digest(args.input_proof/entry['file']) == entry['sha256'], 'source oracle changed')
    replay = load(args.replay/'proof.json')
    require(replay['passed'] and not replay['vendor_loaded'] and not replay['native_encoder_called']
            and not replay['process_attached'] and len(replay['domains']) == 2, 'fake replay scope')
    rebuilt = bytearray(raw[:H.size])
    domains = []
    for (meta, expected, counts), info in zip(frozen_plans(raw), replay['domains']):
        require(info['domain'] == meta[0] and info['completed'] and info['status'] == 2 and info['error'] == 0
                and info['worker_finished'] and not info['worker_timed_out'] and not info['helper_pinned']
                and len({info['worker_tid'], info['entry_tid'], info['producer_tid']}) == 3,
                'independent worker completion')
        require(info['vendor_loads'] == info['activations'] == info['records'] == info['output_bytes']
                == info['endpoint_calls'] == info['hooks_installed'] == 0 and not info['host_guard_passed'],
                'fake path must never claim native host/encoding')
        require(info['initializations'] == 1 and info['entry_return'] == 0
                and info['messages'] == meta[2]+1 and info['eof_seen']
                and info['source_rounds'] == meta[2] and info['warm_rounds'] == 20 and info['drain_rounds'] == 30
                and info['processes'] == meta[1] and info['submissions'] == info['fake_calls_verified'] == counts['submissions']
                and info['source_samples'] == counts['source_audio_samples']
                and info['producer_backpressure'] is False and 0 < info['local_high_water'] <= 8,
                'actual dispatch/EOF counts')
        actual = (args.replay/f'domain-{meta[0]}/fake-plan.bin').read_bytes()
        require(actual == expected and parse_plan(actual, meta[1]) == counts, 'PCM/XYZ/flags/Process transcript changed')
        rebuilt.extend(C.pack(*meta))
        rebuilt.extend(actual)
        domains.append(dict(domain=meta[0], worker_tid=info['worker_tid'], entry_tid=info['entry_tid'],
                            producer_tid=info['producer_tid'], full_transcript_byte_identical=True, **counts))
    require(bytes(rebuilt) == raw, 'whole native-tested input not reproduced')
    result = dict(passed=True, selftests_passed=23, independent_source_samples=sum(d['source_audio_samples'] for d in domains),
                  source_input_sha256=sha(raw), reconstructed_input_sha256=sha(rebuilt), domains=domains,
                  dll_sha256=build['dll_sha256'], executable_sha256=build['executable_sha256'],
                  oracle_sha256=digest(Path(__file__)), standalone_MSVC_wire_layout_matches_frozen_GCC=True,
                  native_encoder_called=False, vendor_loaded=False, process_attached=False,
                  audiodg_host_verified=False, real_ABI_initialized=False, real_PCM_encoded=False,
                  caveat='Ordinary-process own DLL fake ABI and rejection tests only. New adapter not yet exercised in audiodg or across processes.')
    (args.output/'proof.json').write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
