"""Independent byte oracle for threaded FAKE-encoder replay. No audio/native calls.

Compare the actual consumer-thread transcript to the already validated and
native-tested MATV4P1 input, not to a new expectation made by the queue code.
Writes only a fresh proof directory; corruptions operate on in-memory copies.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

H = struct.Struct('<8sIIII')
C = struct.Struct('<Q6I')
B = struct.Struct('<III')
O = struct.Struct('<II3f480f')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def digest(path):
    return sha(path.read_bytes())


def parse_plan(raw, rounds):
    offset = submissions = source_samples = 0
    phase_counts = {1: 0, 2: 0, 3: 0}
    source_index = 0
    for _ in range(rounds):
        require(offset+B.size <= len(raw), 'truncated block')
        phase, index, count = B.unpack_from(raw, offset)
        offset += B.size
        require(phase in phase_counts and count <= 64, 'block bounds')
        phase_counts[phase] += 1
        require(index == (source_index if phase == 2 else 0xffffffff), 'source Process sequence')
        if phase == 2:
            source_index += 1
        for _ in range(count):
            require(offset+O.size <= len(raw), 'truncated object')
            values = O.unpack_from(raw, offset)
            require(values[0] > 0 and values[1] in (6, 7, 8), 'object lifecycle')
            if phase == 2 and values[1] != 8:
                source_samples += 480
            offset += O.size
            submissions += 1
    require(offset == len(raw), 'extra data or source Process at EOF')
    return dict(submissions=submissions, source_audio_samples=source_samples,
                source_process_rounds=source_index, preroll_rounds=phase_counts[1], drain_rounds=phase_counts[3])


def frozen_plans(raw):
    require(H.unpack_from(raw) == (b'MATV4P1\0', 1, 2, 400, 480), 'frozen input header')
    offset = H.size
    cases = []
    for domain in (1, 2):
        meta = C.unpack_from(raw, offset)
        offset += C.size
        require(meta[0] == domain and meta[1] == meta[2]+meta[3]+meta[4]
                and meta[3:] == (20, 30, 10, 1), 'frozen case header')
        start = offset
        for _ in range(meta[1]):
            require(offset+B.size <= len(raw), 'frozen block')
            count = B.unpack_from(raw, offset)[2]
            require(count <= 20, 'frozen case bound')
            offset += B.size+count*O.size
            require(offset <= len(raw), 'frozen payload bound')
        plan = raw[start:offset]
        cases.append((meta, plan, parse_plan(plan, meta[1])))
    require(offset == len(raw), 'frozen trailing bytes')
    return cases


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'input', 'input-proof', 'bridge', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    ap.add_argument('--replays', nargs='+', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    build = load(args.build/'build.json')
    require(digest(args.build/'mat-stream-replay.exe').upper() == build['executable_sha256'], 'executable changed')
    for source in build['source_files']:
        require(digest(args.build.parent/source['file']).upper() == source['sha256'], 'build source changed')
    raw = (args.input/'input.bin').read_bytes()
    proof = load(args.input_proof/'proof.json')
    manifest = load(args.input/'manifest.json')
    require(proof['passed'] and sha(raw) == proof['input_sha256'] == manifest['input_sha256']
            and digest(args.input/'manifest.json') == proof['input_manifest_sha256'], 'frozen input proof')
    for item in proof['expected_files']:
        require(digest(args.input_proof/item['file']) == item['sha256'], 'source oracle changed')
    plans = frozen_plans(raw)
    outputs = []
    for replay in args.replays:
        summary = load(replay/'summary.json')
        require(summary['offline_threaded_fake_passed'] and not summary['native_encoder_called']
                and not summary['mat_encoded'] and summary['source_unchanged'] and len(summary['domains']) == 2,
                'fake replay completion')
        rows = []
        rebuilt = bytearray(raw[:H.size])
        for (meta, expected, counts), domain in zip(plans, summary['domains']):
            require(domain['domain'] == meta[0] and domain['completed'] and domain['fault'] == 0
                    and domain['encoder_opens'] == domain['encoder_closes'] == 1 and domain['encoder_completed']
                    and domain['queue_capacity'] == 8 and 0 < domain['queue_high_water'] <= 8, 'worker/domain contract')
            actual = (replay/domain['fake_plan_file']).read_bytes()
            require(sha(actual) == domain['fake_plan_sha256'] and actual == expected,
                    'consumer PCM/XYZ/flags/Process transcript differs from native-tested input')
            observed = parse_plan(actual, domain['scheduled_rounds'])
            require(observed == counts, 'consumer block counts')
            original = args.bridge/f"domain-{meta[0]}-mat-object-input.bin"
            require((replay/domain['source_file']).read_bytes() == original.read_bytes()
                    and digest(original) == domain['source_sha256'], 'bridge source/EOF changed')
            negative = []
            for name, bad in [('truncated_payload', actual[:-1]), ('extra_EOF_Process', actual+B.pack(2, meta[2], 0)),
                              ('source_sequence_gap', B.pack(2, 1, 0)+actual[B.size:])]:
                try:
                    parse_plan(bad, domain['scheduled_rounds'])
                except ValueError:
                    negative.append(name)
                else:
                    raise ValueError('negative control accepted: '+name)
            altered = bytearray(actual)
            altered[len(altered)//2] ^= 1
            require(bytes(altered) != expected, 'byte mutation not detected')
            negative.append('any_PCM_XYZ_flag_byte_change_detected_by_exact_oracle')
            rebuilt.extend(C.pack(*meta))
            rebuilt.extend(actual)
            rows.append(dict(domain=meta[0], transcript_byte_identical=True, source_and_EOF_byte_identical=True,
                             transcript_sha256=sha(actual), negative_tests=negative, **observed))
        require(bytes(rebuilt) == raw, 'whole MATV4P1 reconstructed input differs')
        outputs.append(dict(replay=str(replay.resolve()), paced_worker=summary['paced_worker'],
                            whole_native_tested_input_byte_identical=True,
                            reconstructed_input_sha256=sha(rebuilt), domains=rows))
    result = dict(passed=True, native_encoder_called=False, mat_encoded=False,
                  executable_sha256=build['executable_sha256'], oracle_sha256=digest(Path(__file__)),
                  frozen_native_input_sha256=sha(raw), replays=outputs,
                  scope='Offline threaded fake transcript equals prior native-tested input. No new encoding/RT/Android claim.')
    (args.output/'proof.json').write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
