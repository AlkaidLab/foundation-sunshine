"""Independent prepared probe-input check plus future per-domain source oracle.
No codec/audio calls. Temporary corruption tests invoke only --validate.
"""
import argparse
import json
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from verify_native_quantum_v4_content import check, digest, load

F = struct.Struct('<8sIIIIQ')
Q = struct.Struct('<IIIIqq')
C = struct.Struct('<IIIIfffI')
H = struct.Struct('<8sIIII')
CASE = struct.Struct('<Q6I')
B = struct.Struct('<III')
O = struct.Struct('<II3f480f')


def original_plan(path):
    raw = path.read_bytes()
    magic, ver, rate, quantum, records, _ = F.unpack_from(raw)
    check(magic == b'MATBRG1\0' and (ver, rate, quantum) == (1, 48000, 480), 'original plan dimensions')
    offset = F.size
    source = []
    for index in range(records):
        seq, frames, count, eof, _, _ = Q.unpack_from(raw, offset)
        offset += Q.size
        check(seq == index and frames == 480 and eof == int(index == records-1), 'original frame')
        calls = []
        for _ in range(count):
            identity, _, typ, flags, x, y, z, frames = C.unpack_from(raw, offset)
            offset += C.size
            check(typ == 1 and frames == 480, 'original call')
            calls.append(struct.pack('<II3f', identity, flags, x, y, z) + raw[offset:offset+1920])
            offset += 1920
        if eof:
            check(not calls, 'EOF cannot become an input Process')
        else:
            source.append(calls)
    check(offset == len(raw), 'original EOF')
    return source


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'export', 'probe', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    args = ap.parse_args()
    manifest = load(args.input/'manifest.json')
    raw = (args.input/'input.bin').read_bytes()
    check(digest(args.input/'input.bin') == manifest['input_sha256'] and len(raw) == manifest['input_bytes'], 'prepared input changed')
    check(H.unpack_from(raw) == (b'MATV4P1\0', 1, 2, 400, 480), 'unique prepared format')
    offset = H.size
    counts = dict(source_audio_samples=0, source_process_blocks=0, preroll_blocks=0, drain_blocks=0, unity_submissions=0)
    expected = []
    cases = []
    for ci, info in enumerate(manifest['cases']):
        domain, blocks, source_blocks, preroll, drain, pace, cut = CASE.unpack_from(raw, offset)
        offset += CASE.size
        check(domain == ci+1 and (preroll, drain, pace) == (20, 30, 10)
              and blocks == source_blocks+preroll+drain <= 400, 'case dimensions')
        path = args.export/f'domain-{domain}-mat-object-input.bin'
        source_meta = manifest['source_export_files'][ci]
        check(path.stat().st_size == source_meta['bytes'] and digest(path) == source_meta['sha256'], 'original export changed')
        source = original_plan(path)
        check(len(source) == source_blocks, 'source native frame count')
        first = source[cut]
        ids = [O.unpack(row)[0] for row in first]
        check(len(ids) == len(set(ids)) and all(O.unpack(row)[1] == 7 for row in first), 'first source identities')
        id_to_row = {identity: n for n, identity in enumerate(ids)}
        audio = np.zeros((len(ids), blocks*480), dtype='<f4')
        xyz = np.full((len(ids), blocks, 3), np.nan, dtype='<f4')
        segments = [dict(generation=n, id=identity, created_block=cut, source_start_block=None, end_block=None) for n, identity in enumerate(ids)]
        live = set()
        mapping = []
        source_index = 0
        for scheduled in range(blocks):
            phase, native_index, count = B.unpack_from(raw, offset)
            offset += B.size
            check(count <= 20, 'prepared object bound')
            desired_phase = 2 if scheduled < cut else 1 if scheduled < cut+20 else 2 if scheduled < source_blocks+20 else 3
            check(phase == desired_phase, 'preroll/source/drain order')
            calls = []
            for _ in range(count):
                check(offset+O.size <= len(raw), 'truncated prepared object')
                calls.append(raw[offset:offset+O.size])
                offset += O.size
            if phase == 1:
                check(native_index == 0xffffffff, 'warmup mislabeled source')
                expected_calls = [struct.pack('<II', O.unpack(row)[0], 7 if scheduled == cut else 6) + row[8:20] + bytes(1920) for row in first]
                counts['preroll_blocks'] += 1
            elif phase == 2:
                check(native_index == source_index, 'source frame dropped/reordered')
                expected_calls = list(source[source_index])
                if source_index == cut:
                    expected_calls = [row[:4] + struct.pack('<I', 6) + row[8:] for row in expected_calls]
                counts['source_process_blocks'] += 1
                source_index += 1
            else:
                check(native_index == 0xffffffff and not live, 'drain with source label/live objects')
                expected_calls = []
                counts['drain_blocks'] += 1
            check(calls == expected_calls, 'prepared calls differ from source PCM/XYZ or prescribed zero warmup')
            for call in calls:
                identity, flags, x, y, z, *_ = O.unpack(call)
                row = id_to_row[identity]
                pcm = np.frombuffer(call, dtype='<f4', count=480, offset=20)
                check(np.all(np.isfinite(pcm)) and all(np.isfinite((x,y,z))), 'nonfinite prepared data')
                if flags == 7:
                    check(identity not in live, 'duplicate create')
                    live.add(identity)
                else:
                    check(identity in live, 'update/end before create')
                xyz[row, scheduled] = (x,y,z)
                if flags == 8:
                    check(np.count_nonzero(pcm) == 0, 'end contains source audio')
                    live.remove(identity)
                    segments[row]['end_block'] = scheduled
                else:
                    audio[row, scheduled*480:(scheduled+1)*480] = pcm
                if phase == 2 and flags != 8:
                    counts['source_audio_samples'] += 480
                    if segments[row]['source_start_block'] is None:
                        segments[row]['source_start_block'] = scheduled
                counts['unity_submissions'] += 1
            mapping.append(dict(scheduled_block=scheduled, phase={1:'preroll',2:'source',3:'drain'}[phase],
                                source_frame=None if native_index == 0xffffffff else native_index, submissions=len(calls)))
        check(not live and source_index == source_blocks and mapping == info['mapping'], 'source map/lifetime incomplete')
        for segment in segments:
            check(segment['source_start_block'] is not None and segment['end_block'] is not None, 'missing source start/end')
            segment['frames'] = (segment['end_block']-segment['source_start_block'])*480
        check(sum(s['frames'] for s in segments) == info['source_audio_samples'], 'audio segments lost')
        expected.append((domain, audio, xyz))
        cases.append(dict(domain=domain, scheduled_blocks=blocks, source_process_blocks=source_blocks,
                          preroll_insert_before_source_frame=cut, preroll_blocks=20, drain_blocks=30, segments=segments))
    check(offset == len(raw) and counts == dict(source_audio_samples=384000, source_process_blocks=512,
                                               preroll_blocks=40, drain_blocks=60, unity_submissions=863), 'whole input counters')
    negative = []
    with tempfile.TemporaryDirectory(prefix='mat-v4-input-check-') as temp:
        temp = Path(temp)
        for name, data in [('wrong-magic', bytearray(raw)), ('too-many-objects', bytearray(raw))]:
            if name == 'wrong-magic':
                data[0] ^= 1
            else:
                struct.pack_into('<I', data, H.size+CASE.size+B.size-4, 21)
            path = temp/(name+'.bin')
            path.write_bytes(data)
            completed = subprocess.run([str(args.probe.resolve()), '--validate', str(path), str(temp/(name+'-out'))], capture_output=True, text=True)
            check(completed.returncode != 0, 'native input parser accepted '+name)
            negative.append(name+'_rejected')
    args.output.mkdir(parents=True, exist_ok=False)
    for domain, audio, xyz in expected:
        np.savez(args.output/f'domain-{domain}-expected.npz', audio=audio, windows_xyz=xyz, sample_rate=48000)
    result = dict(passed=True, input_sha256=digest(args.input/'input.bin'), input_manifest_sha256=digest(args.input/'manifest.json'),
        oracle_sha256=digest(Path(__file__)), probe_sha256=digest(args.probe), cases=cases, counts=counts,
        source_pcm_and_xyz_byte_identical=True, source_gain_applied_again=False,
        source_eof_not_encoded_as_process=True, native_encoder_called=False, source_audio_trimmed=False,
        negative_tests=negative, expected_files=[dict(file=f'domain-{domain}-expected.npz',sha256=digest(args.output/f'domain-{domain}-expected.npz')) for domain,_,_ in expected])
    (args.output/'proof.json').write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
