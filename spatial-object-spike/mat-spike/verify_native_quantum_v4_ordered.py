"""Read-only, bounded native v4 oracle for the approved two-source short test.

Does not certify source PCM by its pre-call tag. Startup mismatches are retained
and reported, never repaired by dividing by a fitted envelope or recorded gain.
"""
import argparse
import collections
import csv
import hashlib
import json
import math
import re
import struct
from pathlib import Path

import numpy as np
from verify_quantum_v4_startup import PE, fade_from_disassembly

HEADER = struct.Struct('<IIQqIIfffffIQIIIIQQQQQQQIIIiII')
assert HEADER.size == 160


def check(okay, message):
    if not okay:
        raise ValueError(message)


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_pcm(variant, which, tick):
    frequency = (783.99 if which else 523.25) + variant * 100.0
    samples = tick * 480 + np.arange(480, dtype=np.float64)
    return (.2 * np.sin(2.0 * math.pi * frequency * samples / 48000.)).astype('<f4')


def source_xyz(variant, which, ticks):
    t = np.asarray(ticks, dtype=np.float64) * .01
    phase = which + variant * 2.0
    return np.column_stack((np.sin(t * 1.1 + phase),
                           .5 * np.sin(t * .7 + phase),
                           -np.cos(t * 1.1 + phase))).astype('<f4')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'parser-proof', 'output'):
        ap.add_argument('--' + name, type=Path, required=True)
    args = ap.parse_args()
    engine = PE(Path('C:/Windows/System32/audioeng.dll'),
                '3936BFFDC4AA365E74C9995359F2821AAF759A14155A180CFA95764962B9F1D3')
    session = PE(Path('C:/Windows/System32/audioses.dll'),
                 '95AF7710EC1A13252283B3740A3EFEEB7C014790F99DC9404D6AEC426E5C6C4E')
    engine.guard(0x6d4d0, '48c7819000000010000000')
    engine.guard(0xfa332, 'e8d99ef4ff')
    exponent = struct.unpack('<f', engine.at(0x106d9c, 4))[0]
    check(exponent == 1.75, 'unverified fade exponent')
    startup_fade = fade_from_disassembly(480, exponent)
    capture = args.run / 'capture'
    native = load(capture / 'summary.json')
    parsed = load(args.parser_proof / 'summary.json')
    launch = load(args.run / 'launch.json')
    exited = load(args.run / 'exit.json')
    check(exited['completed'] and exited['collector_exited'] and exited['collector_exit'] == 0
          and not exited['audited_engine_exited'], 'launcher/engine exit')
    check(native['status'] == 2 and native['original_bytes_restored']
          and all(native[k] == 0 for k in ('error', 'invalid', 'dropped_full', 'dropped_busy',
                                          'extended_calls', 'unbound_after_arming')),
          'native integrity/restoration')
    check(parsed['passed'] and parsed['source_kind'] == 'native-pre-submit-paired'
          and load(capture / 'format.json')['pcm_stage'] == 'before-original-submit', 'v4 provenance')
    check(exited['audiodg_pids_after'] == [launch['audiodg_pid_before']], 'engine changed')
    check(launch['source_order'] == 'GetBuffer -> SetPosition -> SetVolume'
          and launch['source_exe_sha256'] == 'F7154A456EFB672C62E2F9742B0C052F9D2BCB228B1AADE9A6C25BBA080A793F',
          'unverified ordered source provenance')
    check(digest(Path(launch['source_exe'])).upper() == launch['source_exe_sha256'], 'source binary changed')
    check(digest(args.run.parent / 'run-quantum-v4-ordered-once.ps1').upper() == launch['launcher_sha256'], 'launcher changed')
    check(digest(args.run.parent.parent / 'spatial_probe_ordered.cpp').upper() == launch['source_ordered_cpp_sha256'], 'ordered source changed')
    check(digest(args.run.parent.parent / 'spatial_probe.cpp').upper() == launch['source_legacy_include_sha256'], 'legacy include changed')
    source_logs = []
    pattern = (r'EMITTER PID=(\d+) variant=(\d+) objects=(\d+) blocks=(\d+) '
               r'framesPerObject=(\d+) rate=(\d+) duration=([\d.]+)')
    for index, expected in ((1, (0, 2, 300, 144000, 48000)), (2, (1, 1, 200, 96000, 48000))):
        match = re.search(pattern, (args.run / f'source-{index}.stdout.txt').read_text(encoding='utf-8-sig'))
        check(match is not None, 'missing completed source log')
        check('SOURCE_ORDER GetBuffer -> SetPosition -> SetVolume' in
              (args.run / f'source-{index}.stdout.txt').read_text(encoding='utf-8-sig'), 'ordered source marker missing')
        values = tuple(map(int, match.groups()[:6]))
        check(values[1:] == expected, 'unexpected source program')
        source_logs.append(dict(zip(('pid', 'variant', 'objects', 'blocks', 'frames_per_object', 'rate'), values)))
    check(source_logs[0]['pid'] != source_logs[1]['pid'], 'streams must use separate source processes')
    exits = load(args.run / 'source-exits.json')
    check(len(exits) == 2 and all(row['exit'] == 0 for row in exits), 'source exit failure')
    paths = [capture / row['file'] for row in parsed['source_files']]
    manifest = [dict(file=p.name, bytes=p.stat().st_size, sha256=digest(p)) for p in paths]
    check(manifest == parsed['source_files'], 'source manifest changed')

    # Independently pair every return/pass and track live domain identities.
    domains = {}
    active = set()
    sequence = 0
    previous_qpc = -1
    quanta = []
    snapshots = []
    event_counts = collections.Counter()
    properties = collections.Counter()
    for path in paths:
        with path.open('rb') as stream:
            while raw := stream.read(HEADER.size):
                check(len(raw) == HEADER.size, 'short header')
                h = HEADER.unpack(raw)
                check(h[0] == 0x3451424f and h[13] == 4 and h[12] == sequence
                      and h[3] >= previous_qpc and h[17] == 0 and h[29] == 0, 'event identity/order/loss')
                sequence += 1
                previous_qpc = h[3]
                check(0 <= h[1] <= 2048, 'PCM bounds')
                payload = stream.read(h[1] * 4)
                check(len(payload) == h[1] * 4, 'short PCM')
                kind, domain, pass_id, call = h[14], h[21], h[22], h[23]
                event_counts[kind] += 1
                if kind == 1:
                    check(domain not in domains and h[28] == 1 and h[16] == 4, 'domain begin')
                    domains[domain] = dict(identity=h[18:21], begin=h, end=None,
                                           passes=[], current=None, sample=0, pass_id=0)
                    active.add(domain)
                check(domain in active, 'retired/unknown domain')
                d = domains[domain]
                check(d['identity'] == h[18:21], 'domain identity changed')
                if kind == 2:
                    check(d['current'] is None and h[28] in (2, 3, 4, 5), 'domain end')
                    d['end'] = h
                    active.remove(domain)
                elif kind == 3:
                    check(d['current'] is None and pass_id == d['pass_id'] + 1 and h[24] == 480, 'pass begin/frames')
                    d['pass_id'] = pass_id
                    d['current'] = dict(domain=domain, pass_id=pass_id, sample_start=d['sample'],
                                        frames=h[24], objects=[], processed=False, thread=h[11], begin=h)
                elif kind in (4, 5, 6, 7):
                    q = d['current']
                    check(q is not None and q['pass_id'] == pass_id and q['thread'] == h[11], 'pass/thread binding')
                    if kind == 4:
                        check(not q['processed'] and call == len(q['objects']) + 1
                              and (not q['objects'] or q['objects'][-1]['result'] is not None)
                              and h[1] == 480 and h[5] == 1 and h[16] == 1
                              and h[27] == -2147483638, 'pre-submit snapshot')
                        pcm = np.frombuffer(payload, dtype='<f4').copy()
                        check(np.all(np.isfinite(pcm)), 'nonfinite PCM')
                        item = dict(header=h, pcm=pcm, result=None)
                        q['objects'].append(item)
                        snapshots.append(item)
                        properties[h[15]] += 1
                    elif kind == 5:
                        check(not q['processed'] and q['objects'] and call == len(q['objects'])
                              and q['objects'][-1]['result'] is None and h[16] == 2 and h[27] == 0,
                              'Submit return mismatch')
                        q['objects'][-1]['result'] = h
                    elif kind == 6:
                        check(not q['processed'] and h[24] == q['frames'] and h[27] == 0
                              and h[25] <= h[26] and h[16] == 2
                              and all(o['result'] is not None for o in q['objects']), 'Process return mismatch')
                        q['processed'] = True
                        q['process'] = h
                    else:
                        check(q['processed'] and h[16] == 2 and h[27] == 0, 'incomplete pass')
                        q['end'] = h
                        quanta.append(q)
                        d['passes'].append(q)
                        d['sample'] += q['frames']
                        d['current'] = None
                if kind != 4:
                    check(h[1] == 0, 'control PCM payload')
    check(not active and len(domains) == 2 and sequence == native['blocks'], 'domain closure/count')
    check(len(snapshots) == native['submit_results'] == 800 and len(quanta) == native['process_results'],
          'native footer mismatch')
    check(sum(o['header'][1] for o in snapshots) == native['frames'] == 384000, 'sample count')
    check(len(quanta) == parsed['completed_quanta'] and parsed['unknown_offset_objects'] == 0, 'parser count/layout')

    matched = []
    pipeline_samples = pipeline_exact = 0
    max_pipeline_error = 0.0
    first_boundaries = []
    first_envelopes = []
    steady_samples = steady_exact = total_exact = 0
    max_raw_error = max_steady_error = max_xyz_error = 0.0
    post_gain_error = 0.0
    expected_quanta_csv = []
    domain_reports = []
    identity_map = {}
    for domain, source in zip(sorted(domains), source_logs):
        d = domains[domain]
        variant, object_count, blocks = source['variant'], source['objects'], source['blocks']
        audible = [q for q in d['passes'] if q['objects']]
        check(len(audible) == blocks, 'phase coverage')
        first_pass = audible[0]['pass_id']
        check([q['pass_id'] for q in audible] == list(range(first_pass, first_pass + blocks)), 'internal source pass gap')
        empty_count = len(d['passes']) - len(audible)
        check(all(not q['objects'] for q in d['passes'][:first_pass-1]), 'leading idle passes')
        check(d['end'][28] == 2 and d['end'][16] == 1, 'expected observed Reset invalidation')
        for q in d['passes']:
            expected_quanta_csv.append((domain, q['pass_id'], q['sample_start'], 480, len(q['objects']), 1))
        seen = collections.defaultdict(list)
        for ordinal, q in enumerate(audible):
            check(len(q['objects']) == object_count, 'object count changes within source phase')
            tick_set = set()
            for item in q['objects']:
                h, pcm = item['header'], item['pcm']
                which = 0 if h[9] == float(np.float32(.1)) else 1 if h[9] == float(np.float32(.07)) else -1
                check(0 <= which < object_count, 'source volume identity')
                identity = (domain, h[2], h[4], h[5])
                check(identity_map.setdefault(identity, which) == which, 'source identity rebound')
                xyz = np.asarray(h[6:9], dtype=np.float64)
                candidates = source_xyz(variant, which, np.arange(blocks))
                errors = np.max(np.abs(candidates.astype(np.float64) - xyz), axis=1)
                tick = int(np.argmin(errors))
                expected_xyz = candidates[tick]
                check(tick == ordinal and float(errors[tick]) == 0.0, 'XYZ source offset/gap, including first block')
                max_xyz_error = max(max_xyz_error, float(errors[tick]))
                check(h[15] == (7 if tick == 0 else 14 if tick == blocks - 1 else 6), 'create/update/EOS flags')
                expected = source_pcm(variant, which, tick)
                error = float(np.max(np.abs(expected.astype(np.float64) - pcm.astype(np.float64))))
                exact = int(np.count_nonzero(expected.view('<u4') == pcm.view('<u4')))
                total_exact += exact
                max_raw_error = max(max_raw_error, error)
                pipeline_expected = expected * startup_fade if tick == 0 else expected
                pipeline_error = float(np.max(np.abs(pipeline_expected.astype(np.float64) - pcm.astype(np.float64))))
                pipeline_bit_exact = int(np.count_nonzero(pipeline_expected.view('<u4') == pcm.view('<u4')))
                check(pipeline_bit_exact == 480 and pipeline_error == 0, 'PCM differs from fixed native input model')
                pipeline_samples += 480
                pipeline_exact += pipeline_bit_exact
                max_pipeline_error = max(max_pipeline_error, pipeline_error)
                post_gain = expected * np.float32(np.float32(h[9]) * np.float32(h[10]))
                post_error = float(np.max(np.abs(post_gain.astype(np.float64) - pcm.astype(np.float64))))
                if ordinal:
                    check(exact == 480 and error == 0, 'steady PCM is not bit-exact ungained source')
                    check(post_error > .1, 'must distinguish raw from previously gained PCM')
                    steady_samples += 480
                    steady_exact += exact
                    max_steady_error = max(max_steady_error, error)
                    post_gain_error = max(post_gain_error, post_error)
                else:
                    check(error > .1 and exact < 480, 'startup mismatch disappeared; update bounded oracle')
                    valid = np.abs(expected) > 1e-5
                    ratio = pcm[valid].astype(np.float64) / expected[valid].astype(np.float64)
                    check(np.all(ratio >= -1e-7) and np.all(ratio <= 1.000001)
                          and np.min(np.diff(ratio)) >= -1e-6, 'startup differs from diagnostic attenuation pattern')
                    full_ratio = np.full(480, np.nan)
                    full_ratio[valid] = ratio
                    first_envelopes.append(full_ratio)
                    first_boundaries.append(dict(domain=domain, source_object=which, event_sequence=h[12],
                        observed_xyz=list(h[6:9]), source_xyz=[float(x) for x in expected_xyz],
                        properties=h[15], samples=480, bit_exact_source_samples=exact,
                        bit_exact_encoder_input_model_samples=pipeline_bit_exact,
                        max_source_pcm_error=error, monotonic_attenuation_compatible=True,
                        attenuation_at_samples={str(n): float(full_ratio[n]) for n in (1, 120, 240, 479)},
                        same_call_upstream_probe_performed=False, source_bit_exact=False, source_xyz_exact=True))
                seen[which].append(tick)
                tick_set.add(tick)
                matched.append((h[12], domain, h[22], h[23], which, tick, exact, error,
                                float(np.max(np.abs(expected_xyz.astype(np.float64) - xyz))), ordinal == 0))
            check(len(tick_set) == 1, 'Process mixed source ticks')
        check(len(seen) == object_count and all(ticks == list(range(blocks)) for ticks in seen.values()), 'source coverage')
        domain_reports.append(dict(domain=domain, variant=variant, source_pid=source['pid'],
            renderer=d['identity'][0], encoder=d['identity'][1], implementation=d['identity'][2],
            process_passes=len(d['passes']), nonempty_passes=len(audible), empty_passes=empty_count,
            source_blocks=blocks, object_count=object_count, per_domain_sample_frames=d['sample'],
            first_nonempty_sample_start=audible[0]['sample_start'], first_source_block=0,
            last_source_block=blocks - 1, no_missing_object_packets=True, tail_present=True,
            domain_end_reason='Reset (observed before original call, not Reset return proof)'))
    check(len(identity_map) == 3, 'expected three observed object identities')
    with (args.parser_proof / 'quanta.csv').open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    actual_quanta_csv = [tuple(int(row[k]) for k in ('domain', 'pass', 'sample_start', 'frames', 'objects', 'full_span_layout_ready')) for row in rows]
    check(actual_quanta_csv == expected_quanta_csv, 'C++ per-domain input clock/content mismatch')
    with (args.parser_proof / 'objects.csv').open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    check(len(rows) == len(snapshots), 'C++ object count')
    generations = {}
    for row, item in zip(rows, snapshots):
        h = item['header']
        check(tuple(int(row[k]) for k in ('domain', 'pass', 'call', 'host', 'object_id')) == (h[21], h[22], h[23], h[2], h[4]), 'C++ object identity')
        check(int(row['position_state']) == 1 and int(row['offset']) == 0, 'position state/offset')
        for column, value in zip(('x', 'y', 'z', 'volume', 'call_gain'), (*h[6:9], h[9], h[10])):
            check(abs(float(row[column]) - value) <= 1e-8, 'C++ state differs from pre-call payload')
        key = (h[21], h[4])
        check(generations.setdefault(key, int(row['generation'])) == int(row['generation']), 'generation drift')
    check(sorted(g for (domain, _), g in generations.items() if domain == 1) == [1, 2]
          and [g for (domain, _), g in generations.items() if domain == 2] == [1], 'new domain cache not fresh')
    common_ramp_error = max(float(np.nanmax(np.abs(a - first_envelopes[0]))) for a in first_envelopes[1:])
    check(common_ramp_error < 3e-7, 'startup envelopes do not agree across independent frequencies')
    check([dict(file=p.name, bytes=p.stat().st_size, sha256=digest(p)) for p in paths] == manifest, 'source changed during proof')
    args.output.mkdir(parents=True, exist_ok=False)
    with (args.output / 'object-source-match.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(('sequence', 'domain', 'pass', 'call', 'source_object', 'source_block',
                         'bit_exact_source_samples', 'max_source_pcm_error', 'max_source_xyz_error', 'startup_boundary'))
        writer.writerows(matched)
    report = dict(capture_integrity_passed=True, source_unchanged=True, source_files=manifest,
        oracle_script_sha256=digest(Path(__file__)),
        audioeng_sha256=digest(engine.path), audioses_sha256=digest(session.path),
        fade_model_module_sha256=digest(Path(__file__).parent / 'verify_quantum_v4_startup.py'),
        shared_formula_module_sha256=digest(Path(__file__).parent / 'verify_native_quantum_v4_content.py'),
        fade_lut_segments=16, fade_exponent=exponent, fitted_parameters=False,
        source_program_cpp_sha256=digest(args.run.parent.parent / 'spatial_probe.cpp'),
        approved_launcher_sha256=digest(args.run.parent / 'run-quantum-v4-ordered-once.ps1'),
        ordered_source_cpp_sha256=launch['source_ordered_cpp_sha256'], source_exe_sha256=launch['source_exe_sha256'],
        deployed_helper_sha256=launch['helper_sha256'], collector_sha256=launch['collector_sha256'],
        controlled_test_analysis_passed=True, all_source_pcm_and_xyz_bit_exact=False,
        complete_source_contract_passed=False,
        complete_encoder_input_capture_contract_passed=True,
        all_encoder_input_model_pcm_and_xyz_bit_exact=True,
        first_block_coordinates_native_verified=True,
        encoder_input_model_samples_checked=pipeline_samples, encoder_input_model_bit_exact_samples=pipeline_exact,
        max_encoder_input_model_pcm_error=max_pipeline_error,
        native_pre_submit_gain_status='steady raw; first block matches PE-derived Windows fade before object volume/callGain',
        records=sequence, completed_quanta=len(quanta), submit_pairs=len(snapshots), domain_count=len(domains),
        domains=domain_reports, source_phases=source_logs, property_flags=dict(properties),
        object_packets=800, object_samples=384000, total_bit_exact_source_samples=total_exact,
        steady_object_packets=797, steady_samples_checked=steady_samples, steady_bit_exact_source_samples=steady_exact,
        max_steady_pcm_error=max_steady_error, max_steady_xyz_error=max_xyz_error,
        max_all_source_pcm_error=max_raw_error, max_steady_error_against_recorded_combined_gain=post_gain_error,
        no_internal_packet_gaps=True, both_new_source_streams_captured=True, native_reset_domains_observed=True,
        fresh_per_domain_cache_and_input_clock_verified=True, first_blocks_preserved_not_discarded=True,
        startup_xyz_anomalous_objects=0, startup_faded_objects=3, startup_faded_samples=1440, startup_boundaries=first_boundaries,
        max_shared_startup_attenuation_disagreement=common_ramp_error,
        raw_audio_repaired_or_inverse_gained=False, original_hook_bytes_restored=True,
        restoration_direct_readback_scope='collector rereads renderer prologue; DLL status checks saved 16 bytes at all six entries',
        capture_seconds=native['elapsed_seconds'], full_game_mix_verified=False, twenty_object_native_verified=False,
        mat_encoded=False, android_atmos_verified=False, cross_domain_sample_sync_verified=False,
        scope='One controlled endpoint/version, 2-object stream then 1-object new stream. Not a universal resident certification.',
        conclusion='Ordered source native test: XYZ matches from block 0 for both streams; every PCM sample matches the fixed Windows input model (first-block native fade, then raw source). Preserve first fade; this is not bit-exact unprocessed application PCM, MAT encoding, global synchronization or Android proof.')
    (args.output / 'proof.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
