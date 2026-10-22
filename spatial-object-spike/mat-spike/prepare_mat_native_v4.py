"""Package verified C++ MATBRG1 plans for a new bounded native probe. No codec.

MATV4P1 is deliberately NOT MATLIF1 or MAT. Inserted warmup/drain and source
frame mapping remain explicit. No source audio trimming or inverse processing.
"""
import argparse
import json
import struct
from pathlib import Path

import numpy as np
from verify_native_quantum_v4_content import check, digest, load
from verify_cpp_native_v4 import FILE, FRAME, CALL

HEAD = struct.Struct('<8sIIII')
CASE = struct.Struct('<Q6I')
BLOCK = struct.Struct('<III')
OBJECT = struct.Struct('<II3f480f')
PREROLL, DRAIN = 20, 30


def read_plan(path):
    data = path.read_bytes()
    check(len(data) >= FILE.size, 'short plan')
    magic, version, rate, frames, records, _ = FILE.unpack_from(data)
    check(magic == b'MATBRG1\0' and (version, rate, frames) == (1, 48000, 480), 'plan format')
    offset = FILE.size
    source = []
    live = set()
    for index in range(records):
        check(offset + FRAME.size <= len(data), 'short frame')
        seq, size, count, eof, _, _ = FRAME.unpack_from(data, offset)
        offset += FRAME.size
        check(seq == index and size == 480 and count <= 20 and eof == int(index == records-1), 'frame order/EOF')
        calls = []
        for _ in range(count):
            check(offset + CALL.size + 480*4 <= len(data), 'short call')
            identity, _, typ, flags, x, y, z, size = CALL.unpack_from(data, offset)
            offset += CALL.size
            pcm = data[offset:offset+480*4]
            offset += 480*4
            check(typ == 1 and size == 480 and flags in (7, 6, 8) and identity > 0, 'object format')
            values = np.frombuffer(pcm, dtype='<f4')
            check(np.all(np.isfinite(values)) and np.max(np.abs(values)) <= 1
                  and all(np.isfinite((x, y, z))), 'invalid PCM/XYZ')
            if flags == 7:
                check(identity not in live, 'duplicate create')
                live.add(identity)
            else:
                check(identity in live, 'update/end without create')
            if flags == 8:
                check(np.count_nonzero(values) == 0, 'end PCM not zero')
                live.remove(identity)
            calls.append((identity, flags, (x, y, z), pcm))
        if eof:
            check(not calls and not live, 'EOF must close no live objects for this source fixture')
        else:
            source.append(calls)
    check(offset == len(data), 'trailing plan bytes')
    return source


def package_case(domain, source):
    first = next((n for n, calls in enumerate(source) if calls), None)
    check(first is not None and all(not c for c in source[:first]), 'no initial create block')
    initial = source[first]
    check(1 <= len(initial) <= 20 and all(row[1] == 7 for row in initial), 'initial calls must all create')
    initial_ids = {row[0] for row in initial}
    blocks = []
    for index, calls in enumerate(source):
        if index == first:
            for warm in range(PREROLL):
                rows = [(ident, 7 if warm == 0 else 6, xyz, bytes(480*4)) for ident, _, xyz, _ in initial]
                blocks.append((1, 0xffffffff, rows))
        converted = []
        for ident, flags, xyz, pcm in calls:
            if index == first:
                check(ident in initial_ids and flags == 7, 'initial identity mismatch')
                flags = 6  # Same object already created during explicit warmup.
            else:
                check(flags != 7, 'later create requires separate probe design, not implicit warmup')
            converted.append((ident, flags, xyz, pcm))
        blocks.append((2, index, converted))
    blocks.extend((3, 0xffffffff, []) for _ in range(DRAIN))
    check(len(blocks) <= 400, 'case exceeds fixed capacity')
    data = bytearray(CASE.pack(domain, len(blocks), len(source), PREROLL, DRAIN, 10, first))
    mapping = []
    pcm_sha = __import__('hashlib').sha256()
    source_audio_samples = 0
    for schedule, (phase, index, rows) in enumerate(blocks):
        data.extend(BLOCK.pack(phase, index, len(rows)))
        for ident, flags, xyz, pcm in rows:
            data.extend(struct.pack('<II3f', ident, flags, *xyz) + pcm)
            if phase == 2 and flags != 8:
                pcm_sha.update(pcm)
                source_audio_samples += 480
        mapping.append(dict(scheduled_block=schedule, phase={1:'preroll', 2:'source', 3:'drain'}[phase],
                            source_frame=None if index == 0xffffffff else index, submissions=len(rows)))
    return bytes(data), dict(domain=domain, source_process_blocks=len(source), preroll_blocks=PREROLL,
        drain_blocks=DRAIN, scheduled_process_blocks=len(blocks), preroll_insert_before_source_frame=first,
        original_eof_process_calls=0, source_audio_samples=source_audio_samples,
        source_scaled_pcm_sha256=pcm_sha.hexdigest(), initial_ids=sorted(initial_ids), mapping=mapping,
        volume=1, call_gain=1, original_source_create_rewritten_to_update_only_after_explicit_warmup=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--export', type=Path, required=True)
    ap.add_argument('--proof', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    proof, summary = load(args.proof), load(args.export/'summary.json')
    check(proof['passed'] and proof['independent_golden_bytes_identical'] and not proof['mat_encoded'], 'verified input plan required')
    domains = sorted(proof['domains'], key=lambda row: row['domain'])
    check([r['domain'] for r in domains] == [1, 2], 'bounded two-domain test only')
    data = bytearray(HEAD.pack(b'MATV4P1\0', 1, 2, 400, 480))
    cases = []
    files = []
    for row in domains:
        path = args.export/f"domain-{row['domain']}-mat-object-input.bin"
        check(path.stat().st_size == row['bytes'] and digest(path) == row['sha256'], 'plan changed')
        source = read_plan(path)
        check(len(source) == row['process_passes'], 'source frame count')
        case, info = package_case(row['domain'], source)
        data.extend(case)
        cases.append(info)
        files.append(dict(file=path.name, bytes=path.stat().st_size, sha256=digest(path)))
    check(sum(c['source_audio_samples'] for c in cases) == 384000, 'source samples lost')
    check(sum(c['scheduled_process_blocks'] for c in cases) == 612, 'bounded schedule changed')
    check(summary['gain_policy'].startswith('F32(pcm * F32(volume * callGain))'), 'gain policy')
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output/'input.bin').write_bytes(data)
    manifest = dict(format='MATV4P1', version=1, input_sha256=digest(args.output/'input.bin'), input_bytes=len(data),
        source_export_files=files, bridge_proof_sha256=digest(args.proof), bridge_summary_sha256=digest(args.export/'summary.json'),
        script_sha256=digest(Path(__file__)), cases=cases, scheduled_seconds=6.12,
        source_audio_samples=384000, source_native_process_blocks=512, inserted_preroll_process_blocks=40,
        inserted_drain_process_blocks=60, source_eof_records_do_not_call_process=True,
        source_audio_trimmed=False, source_gain_applied_again=False, native_encoder_called=False,
        scope='Prepared probe input only; preroll/drain not source audio. Actual startup/tail correctness still needs native output evidence.')
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf8')
    print(json.dumps({k:v for k,v in manifest.items() if k != 'cases'}, indent=2))


if __name__ == '__main__':
    main()
