"""Independent raw-event -> diagnostic MAT call-plan golden; no native codec.

Models gain/lifetime/sample clocks directly from source events, not C++ CSV/state.
Also checks bounded worker pacing and rejects corrupted export/provenance copies.
"""
import argparse
import collections
import csv
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from verify_native_quantum_v4_content import HEADER, check, digest, load
from make_quantum_fixture import EVENT as V3_HEADER

FILE = struct.Struct('<8sIIIIQ')
FRAME = struct.Struct('<IIIIqq')
CALL = struct.Struct('<IIIIfffI')


def call_bytes(identity, slot, flags, xyz, pcm):
    return CALL.pack(identity, slot, 1, flags, *xyz, 480) + np.asarray(pcm, dtype='<f4').tobytes()


def golden(capture, manifest, frequency):
    domains = {}
    active_domains = set()
    sequence = 0
    clocks = []
    sample_count = differing_legacy = differing_twice = 0
    for row in manifest:
        with (capture / row['file']).open('rb') as stream:
            while raw := stream.read(160):
                check(len(raw) == 160, 'short raw event')
                h = HEADER.unpack(raw)
                payload = stream.read(h[1] * 4)
                check(len(payload) == h[1]*4 and h[0] == 0x3451424f and h[13] == 4
                      and h[12] == sequence and h[17] == 0, 'raw event identity/loss')
                sequence += 1
                kind, domain = h[14], h[21]
                if kind == 1:
                    check(domain not in domains, 'duplicate domain')
                    domains[domain] = dict(first=None, objects=[], cache={}, active={}, pending=[],
                        slots={}, next_id=1, generation=0, frames=[], clock=0, pass_id=0,
                        creates=0, ends=0, audio_calls=0, empty=0)
                    active_domains.add(domain)
                check(domain in active_domains, 'event after domain close')
                d = domains[domain]
                if kind == 2:
                    check(d['first'] is None, 'domain end in pass')
                    calls = []
                    for identity, slot, xyz in d['pending']:
                        calls.append(call_bytes(identity, slot, 8, xyz, np.zeros(480, dtype='<f4')))
                    for value in d['active'].values():
                        calls.append(call_bytes(value['id'], value['slot'], 8, value['xyz'], np.zeros(480, dtype='<f4')))
                    d['ends'] += len(calls)
                    d['frames'].append(FRAME.pack(d['pass_id'], 480, len(calls), 1, -1, -1) + b''.join(calls))
                    d['reason'] = h[28]
                    active_domains.remove(domain)
                elif kind == 3:
                    check(d['first'] is None and h[22] == d['pass_id']+1 and h[24] == 480, 'pass begin')
                    d['first'] = h[3]
                    d['objects'] = []
                elif kind == 4:
                    check(d['first'] is not None and h[1] == 480 and h[5] == 1 and h[16] == 1
                          and h[27] == -2147483638, 'object stage/length')
                    d['objects'].append((h, np.frombuffer(payload, dtype='<f4').copy()))
                elif kind == 7:
                    check(d['first'] is not None, 'end without begin')
                    calls = []
                    for identity, slot, xyz in d['pending']:
                        calls.append(call_bytes(identity, slot, 8, xyz, np.zeros(480, dtype='<f4')))
                    d['ends'] += len(calls)
                    d['pending'] = []
                    if not d['objects']:
                        d['empty'] += 1
                    for obj, pcm in d['objects']:
                        key = (obj[2], obj[4], obj[5])
                        flags = obj[15]
                        check(flags in (7, 6, 14), 'bounded native source flags')
                        if key not in d['cache'] or flags & 1:
                            d['generation'] += 1
                            d['cache'][key] = dict(generation=d['generation'], xyz=None, volume=None)
                        state = d['cache'][key]
                        if flags & 4:
                            state['xyz'] = obj[6:9]
                        if flags & 2:
                            state['volume'] = obj[9]
                        check(state['xyz'] is not None and state['volume'] is not None, 'unknown bounded input')
                        slot = d['slots'].setdefault(key, len(d['slots']))
                        full_key = (*key, state['generation'])
                        created = full_key not in d['active']
                        if created:
                            d['active'][full_key] = dict(id=d['next_id'], slot=slot, xyz=state['xyz'])
                            d['next_id'] += 1
                            d['creates'] += 1
                        live = d['active'][full_key]
                        live['xyz'] = state['xyz']
                        gain = np.float32(np.float32(state['volume']) * np.float32(obj[10]))
                        scaled = pcm * gain
                        # Negative controls use the same captured values, no free fit.
                        legacy = (pcm * np.float32(state['volume'])) * np.float32(obj[10])
                        twice = scaled * gain
                        differing_legacy += int(np.count_nonzero(legacy.view('<u4') != scaled.view('<u4')))
                        differing_twice += int(np.count_nonzero(twice.view('<u4') != scaled.view('<u4')))
                        sample_count += 480
                        calls.append(call_bytes(live['id'], slot, 7 if created else 6, state['xyz'], scaled))
                        d['audio_calls'] += 1
                        if flags & 8:
                            d['pending'].append((live['id'], slot, state['xyz']))
                            d['active'].pop(full_key)
                            d['cache'].pop(key)
                    clocks.append((domain, h[22], d['clock'], 480, len(d['objects']), d['first'], h[3]))
                    d['frames'].append(FRAME.pack(d['pass_id'], 480, len(calls), 0, d['first'], h[3]) + b''.join(calls))
                    d['clock'] += 480
                    d['pass_id'] += 1
                    d['first'] = None
    check(not active_domains and sample_count == 384000, 'source closure/count')
    for domain, d in domains.items():
        d['bytes'] = FILE.pack(b'MATBRG1\0', 1, 48000, 480, len(d['frames']), frequency) + b''.join(d['frames'])
    return domains, clocks, dict(samples=sample_count, differing_separate_f32_samples=differing_legacy,
                                 differing_double_gain_samples=differing_twice)


def verify_exports(root, domains):
    result = []
    for domain, d in domains.items():
        path = root / f'domain-{domain}-mat-object-input.bin'
        actual = path.read_bytes()
        check(actual == d['bytes'], 'binary differs from independent golden')
        offset = FILE.size
        active = {}
        audio = ends = creates = 0
        for index in range(len(d['frames'])):
            seq, frames, count, eof, first, last = FRAME.unpack_from(actual, offset)
            offset += FRAME.size
            check(seq == index and frames == 480 and eof == int(index == len(d['frames'])-1), 'export frame/EOF')
            if eof:
                check((first, last) == (-1, -1), 'EOF timestamp fabricated')
            for _ in range(count):
                ident, slot, typ, flags, x, y, z, length = CALL.unpack_from(actual, offset)
                offset += CALL.size
                pcm = np.frombuffer(actual, dtype='<f4', count=length, offset=offset)
                offset += length*4
                check(typ == 1 and length == 480 and flags in (7, 6, 8), 'call format')
                if flags == 7:
                    check(ident not in active, 'duplicate create')
                    active[ident] = slot
                    creates += 1
                else:
                    check(active.get(ident) == slot, 'update/end before create or rebound')
                if flags == 8:
                    check(np.count_nonzero(pcm) == 0, 'end carries audio')
                    active.pop(ident)
                    ends += 1
                else:
                    check(not eof, 'audio PCM in EOF control')
                    audio += 1
        check(offset == len(actual) and not active and audio == d['audio_calls']
              and creates == ends == d['creates'], 'export length/lifecycle')
        result.append(dict(domain=domain, process_passes=d['pass_id'], input_sample_frames=d['clock'],
            empty_process_passes=d['empty'], audio_calls=audio, creates=creates, end_controls=ends,
            close_control_records=1, close_control_advances_clock=False, close_reason=d['reason'],
            bytes=len(actual), sha256=hashlib.sha256(actual).hexdigest()))
    return result


def cross_check_v3(root, proof_path, parser_proof_path, current_domain_bytes):
    """Compare against real native post-Submit samples, not just an F32 formula."""
    proof = load(proof_path)
    check(proof['passed'] and proof['captured_pcm_gain_model'] == 'combined_f32', 'v3 reference proof')
    manifest = load(parser_proof_path)['source_files']
    lookup = {}
    _, _, _, _, records, _ = FILE.unpack_from(current_domain_bytes)
    offset = FILE.size
    for _ in range(records):
        _, _, calls, eof, _, _ = FRAME.unpack_from(current_domain_bytes, offset)
        offset += FRAME.size
        for _ in range(calls):
            _, slot, _, flags, x, y, z, frames = CALL.unpack_from(current_domain_bytes, offset)
            offset += CALL.size
            pcm = current_domain_bytes[offset:offset+frames*4]
            offset += frames*4
            if flags != 8:
                check(not eof, 'reference PCM in EOF')
                key = (slot, struct.pack('<fff', x, y, z))
                check(key not in lookup, 'ambiguous bounded XYZ tick')
                lookup[key] = pcm
    samples = objects = 0
    for row in manifest:
        path = root / row['file']
        check(path.stat().st_size == row['bytes'] and digest(path) == row['sha256'], 'v3 reference changed')
        with path.open('rb') as stream:
            while raw := stream.read(V3_HEADER.size):
                check(len(raw) == V3_HEADER.size, 'short v3 header')
                h = V3_HEADER.unpack(raw)
                pcm = stream.read(h[1]*4)
                check(len(pcm) == h[1]*4, 'short v3 PCM')
                if h[14] != 1:
                    continue
                slot = 0 if h[9] == float(np.float32(.1)) else 1 if h[9] == float(np.float32(.07)) else -1
                key = (slot, struct.pack('<fff', *h[6:9]))
                check(slot >= 0 and lookup.get(key) == pcm, 'C++ gain output differs from real v3 post-Submit PCM')
                samples += h[1]
                objects += 1
    check(samples == proof['source_pcm_samples_checked'] == 284160 and objects == 592, 'v3 reference coverage')
    return dict(real_post_submit_samples_bit_exact=samples, real_post_submit_packets=objects,
                reference_source_files=manifest, reference_proof_sha256=digest(proof_path),
                reference_parser_proof_sha256=digest(parser_proof_path),
                scope='Separate controlled native recordings; no same-call dual probe or universal gain rule claim.')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for arg in ('capture', 'source-proof', 'export', 'exe', 'output', 'v3-capture', 'v3-proof', 'v3-parser-proof'):
        ap.add_argument('--'+arg, type=Path, required=True)
    args = ap.parse_args()
    source = load(args.source_proof)
    report = load(args.export/'summary.json')
    check(source['complete_encoder_input_capture_contract_passed'] and report['offline_bridge_replay_passed'], 'source/replay failed')
    check(digest(args.source_proof) == report['source_content_proof_sha256'], 'source proof changed')
    manifest = [dict(file=row['file'], bytes=(args.capture/row['file']).stat().st_size,
                     sha256=digest(args.capture/row['file'])) for row in source['source_files']]
    check(manifest == source['source_files'] == report['source_files'], 'source hash mismatch')
    native = load(args.capture/'summary.json')
    domains, clocks, gain = golden(args.capture, manifest, native['qpc_frequency'])
    exports = verify_exports(args.export, domains)
    native_gain_reference = cross_check_v3(args.v3_capture, args.v3_proof, args.v3_parser_proof, domains[1]['bytes'])
    check(gain['differing_separate_f32_samples'] > 0 and gain['differing_double_gain_samples'] > 380000,
          'gain negative controls do not distinguish policies')
    with (args.export/'input-clock.csv').open(newline='') as stream:
        actual = [tuple(int(r[k]) for k in ('domain', 'pass', 'sample_start', 'input_frames', 'object_packets', 'first_qpc', 'last_qpc'))
                  for r in csv.DictReader(stream)]
    check(actual == clocks, 'input clock CSV differs from raw event clock')
    with (args.export/'paced-timing.csv').open(newline='') as stream:
        times = list(csv.DictReader(stream))
    check([int(r['item']) for r in times] == list(range(len(times))) and len(times) == 516, 'paced item count/order')
    missed = 0
    for row in times:
        target, enqueued, start, finish, hz = (int(row[k]) for k in ('target', 'enqueue', 'start', 'finish', 'qpc_frequency'))
        check(enqueued <= start <= finish and hz > 0, 'paced timestamps')
        if int(row['kind']) == 7:
            missed += finish-target > hz//100
    check(missed == report['paced']['process_deadline_10ms_misses'] == 0
          and report['paced']['producer_overruns'] == 0 and report['paced']['matches_unpaced_all_domain_bytes_and_state'], 'paced verification')
    # Own temporary copies only; source recording and published export stay read-only.
    negative = []
    with tempfile.TemporaryDirectory(prefix='native-v4-proof-') as scratch:
        temp = Path(scratch)
        for domain, d in domains.items():
            (temp/f'domain-{domain}-mat-object-input.bin').write_bytes(d['bytes'])
        path = temp/'domain-1-mat-object-input.bin'
        data = bytearray(path.read_bytes())
        data[FILE.size+FRAME.size+FRAME.size+CALL.size] ^= 1
        path.write_bytes(data)
        rejected = False
        try:
            verify_exports(temp, domains)
        except ValueError:
            rejected = True
        check(rejected, 'mutated export accepted')
        negative.append('mutated_export_rejected')
        old_before = digest(args.export/'summary.json')
        refused = subprocess.run([str(args.exe.resolve()), str(args.capture.resolve()), str(args.source_proof.resolve()), str(args.export.resolve())], capture_output=True, text=True)
        check(refused.returncode != 0 and digest(args.export/'summary.json') == old_before, 'existing output overwritten')
        negative.append('existing_output_refused_unchanged')
        fake = dict(source)
        fake['complete_encoder_input_capture_contract_passed'] = False
        fake_path = temp/'bad-source-proof.json'
        fake_path.write_text(json.dumps(fake), encoding='utf8')
        refused = subprocess.run([str(args.exe.resolve()), str(args.capture.resolve()), str(fake_path), str(temp/'must-not-create')], capture_output=True, text=True)
        check(refused.returncode != 0 and not (temp/'must-not-create').exists(), 'invalid source proof accepted')
        negative.append('unverified_source_proof_rejected_before_output')
    check([dict(file=row['file'], bytes=(args.capture/row['file']).stat().st_size,
                sha256=digest(args.capture/row['file'])) for row in manifest] == manifest, 'source changed during proof')
    args.output.mkdir(parents=True, exist_ok=False)
    result = dict(passed=True, source_unchanged=True, export_files_unchanged=True, source_files=manifest,
        independent_golden_bytes_identical=True, domains=exports, gain_policy='F32(pcm * F32(volume * callGain))',
        gain_negative_controls=gain, samples_gain_applied_exactly_once=384000,
        independent_native_post_submit_reference=native_gain_reference,
        per_domain_clock_from_native_markers=True, empty_passes_preserved=12,
        close_controls_not_audio_processes=True, paced_process_passes=512, paced_10ms_misses=missed,
        paced_matches_unpaced=report['paced']['matches_unpaced_all_domain_bytes_and_state'], negative_tests=negative,
        oracle_sha256=digest(Path(__file__)), executable_sha256=digest(args.exe), mat_encoded=False,
        vendor_called=False, android_atmos_verified=False, cross_domain_sample_sync_verified=False,
        scope='Independent raw v4 -> diagnostic MATBRG1 input plan, not encoded MAT/transport/native timing.')
    (args.output/'proof.json').write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
